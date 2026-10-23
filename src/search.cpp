#include "search.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>

namespace qb {

static bool startsWith(const std::wstring& s, const wchar_t* p, std::wstring* rest = nullptr) {
    size_t n = wcslen(p);
    if (s.compare(0, n, p) != 0) return false;
    if (rest) *rest = s.substr(n);
    return true;
}

static bool containsAny(std::wstring_view s, const wchar_t* set) {
    return s.find_first_of(set) != std::wstring_view::npos;
}

static bool hasSuffix(const std::wstring& s, const wchar_t* suf) {
    size_t n = wcslen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

bool parseSize(const std::wstring& in, int64_t& out) {
    std::wstring s = fold(trim(in));
    double mult = 1;
    struct { const wchar_t* suf; double m; } units[] = {
        {L"tb", 1099511627776.0}, {L"gb", 1073741824.0}, {L"mb", 1048576.0}, {L"kb", 1024.0},
        {L"t", 1099511627776.0},  {L"g", 1073741824.0},  {L"m", 1048576.0},  {L"k", 1024.0},
        {L"b", 1.0}};
    for (auto& u : units) {
        if (hasSuffix(s, u.suf)) {
            mult = u.m;
            s.resize(s.size() - wcslen(u.suf));
            break;
        }
    }
    s = trim(s);
    if (s.empty()) return false;
    wchar_t* end = nullptr;
    double v = wcstod(s.c_str(), &end);
    if (end == s.c_str() || *end != L'\0' || !(v >= 0) || v * mult > 9.0e18) return false;
    out = static_cast<int64_t>(v * mult);
    return true;
}

bool parseSizeRange(const std::wstring& in, int64_t& mn, int64_t& mx) {
    std::wstring s = trim(in), r;
    int64_t v;
    if (s.empty()) return false;
    if (startsWith(s, L">=", &r)) { if (parseSize(r, v)) { mn = v; mx = -1; return true; } }
    else if (startsWith(s, L"<=", &r)) { if (parseSize(r, v)) { mn = 0; mx = v; return true; } }
    else if (startsWith(s, L">", &r)) { if (parseSize(r, v)) { mn = v + 1; mx = -1; return true; } }
    else if (startsWith(s, L"<", &r)) { if (parseSize(r, v) && v > 0) { mn = 0; mx = v - 1; return true; } }
    size_t sepPos = s.find(L"..");
    size_t sepLen = 2;
    if (sepPos == std::wstring::npos) {
        sepPos = s.find(L'-');
        sepLen = 1;
    }
    if (sepPos != std::wstring::npos && sepPos > 0) {
        int64_t a = 0, b = 0;
        if (parseSize(s.substr(0, sepPos), a) && parseSize(s.substr(sepPos + sepLen), b) && b >= a) {
            mn = a; mx = b;
            return true;
        }
        return false;
    }
    if (parseSize(s, v)) { mn = mx = v; return true; }
    return false;
}

static std::time_t dayStart(std::time_t t, int addDays = 0, int addMonths = 0, int addYears = 0) {
    std::tm tmv{};
    localtime_s(&tmv, &t);
    tmv.tm_hour = tmv.tm_min = tmv.tm_sec = 0;
    tmv.tm_mday += addDays;
    tmv.tm_mon += addMonths;
    tmv.tm_year += addYears;
    tmv.tm_isdst = -1;
    return std::mktime(&tmv);
}

static bool parseDateSpan(const std::wstring& s, std::time_t now, int64_t& start, int64_t& end) {
    if (s == L"today" || s == L"bugun") { start = dayStart(now); end = dayStart(now, 1) - 1; return true; }
    if (s == L"yesterday" || s == L"dun") { start = dayStart(now, -1); end = dayStart(now) - 1; return true; }
    if (s == L"week" || s == L"hafta") { start = dayStart(now, -7); end = -1; return true; }
    if (s == L"month" || s == L"ay") { start = dayStart(now, 0, -1); end = -1; return true; }
    int y = 0, m = 0, d = 0, n = 0;
    if (s.size() == 10 && swscanf(s.c_str(), L"%4d-%2d-%2d%n", &y, &m, &d, &n) == 3 && n == 10 && m >= 1 && m <= 12 && d >= 1 && d <= 31) {
        std::tm tmv{}; tmv.tm_year = y - 1900; tmv.tm_mon = m - 1; tmv.tm_mday = d; tmv.tm_isdst = -1;
        std::time_t t = std::mktime(&tmv);
        if (t == -1) return false;
        start = t; end = dayStart(t, 1) - 1; return true;
    }
    if (s.size() == 7 && swscanf(s.c_str(), L"%4d-%2d%n", &y, &m, &n) == 2 && n == 7 && m >= 1 && m <= 12) {
        std::tm tmv{}; tmv.tm_year = y - 1900; tmv.tm_mon = m - 1; tmv.tm_mday = 1; tmv.tm_isdst = -1;
        std::time_t t = std::mktime(&tmv);
        if (t == -1) return false;
        start = t; end = dayStart(t, 0, 1) - 1; return true;
    }
    if (s.size() == 4 && swscanf(s.c_str(), L"%4d%n", &y, &n) == 1 && n == 4 && y >= 1970) {
        std::tm tmv{}; tmv.tm_year = y - 1900; tmv.tm_mon = 0; tmv.tm_mday = 1; tmv.tm_isdst = -1;
        std::time_t t = std::mktime(&tmv);
        if (t == -1) return false;
        start = t; end = dayStart(t, 0, 0, 1) - 1; return true;
    }
    return false;
}

bool parseDateRange(const std::wstring& in, std::time_t now, int64_t& mn, int64_t& mx) {
    std::wstring s = trim(in);
    if (s.empty()) return false;
    std::wstring op;
    if (startsWith(s, L">=") || startsWith(s, L"<=")) op = s.substr(0, 2);
    else if (startsWith(s, L">") || startsWith(s, L"<")) op = s.substr(0, 1);
    int64_t a = 0, b = 0;
    if (op.empty()) {
        if (!parseDateSpan(s, now, a, b)) return false;
        mn = a; mx = b;
        return true;
    }
    if (!parseDateSpan(s.substr(op.size()), now, a, b)) return false;
    if (op == L">=") { mn = a; mx = -1; return true; }
    if (op == L">") { if (b < 0) return false; mn = b + 1; mx = -1; return true; }
    if (op == L"<=") { if (b < 0) return false; mn = 0; mx = b; return true; }
    if (a <= 0) return false;
    mn = 0; mx = a - 1;
    return true;
}

static bool parseExtToken(const std::wstring& tok, std::vector<std::wstring>& out) {
    std::wstring t;
    if (!startsWith(tok, L"extension:", &t) && !startsWith(tok, L"ext:", &t) &&
        !startsWith(tok, L"uzanti:", &t) && !startsWith(tok, L"*.", &t)) {
        if (tok.size() > 1 && tok[0] == L'.') t = tok.substr(1);
        else return false;
    }
    for (wchar_t& ch : t) {
        if (ch == L',') ch = L'|';
    }
    bool any = false;
    for (std::wstring part : split(t, L'|')) {
        size_t b = part.find_first_not_of(L". ");
        size_t e = part.find_last_not_of(L". ");
        part = (b == std::wstring::npos) ? L"" : part.substr(b, e - b + 1);
        if (part.empty() || containsAny(part, L"\\/:*?\"<>|")) continue;
        out.push_back(part);
        any = true;
    }
    return any;
}

static bool tryRegex(const std::wstring& pat, std::vector<std::wregex>& dst) {
    // ReDoS protection: limit pattern length to 128 characters and check for obvious nested quantifiers.
    if (pat.empty() || pat.size() > 128) return false;
    // Check nested/catastrophic quantifiers like (a+)+, (.*)*, a**
    wchar_t last = 0;
    bool inGroup = false;
    bool groupHasQuant = false;
    for (size_t i = 0; i < pat.size(); ++i) {
        wchar_t c = pat[i];
        if (c == L'(') {
            inGroup = true;
            groupHasQuant = false;
        } else if (c == L')') {
            inGroup = false;
        } else if (c == L'*' || c == L'+') {
            if (last == L'*' || last == L'+') return false; // double repetition like ** or ++
            if (!inGroup && groupHasQuant && last == L')') return false; // (a+)+ or (.*)*
            if (inGroup) groupHasQuant = true;
        } else if (c == L'?') {
            if (last == L'?') return false;
        }
        if (c != L' ') last = c;
    }
    try {
        dst.emplace_back(pat, std::regex::ECMAScript | std::regex::icase | std::regex::optimize);
    } catch (...) {
        return false;
    }
    return true;
}

static std::wstring stripEnclosingQuotes(std::wstring s) {
    s = trim(s);
    if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

Query parseQuery(const std::wstring& text, std::time_t now) {
    Query q;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && iswspace(text[i])) ++i;
        if (i >= text.size()) break;

        size_t j = i;
        std::wstring raw;
        if (text[i] == L'"') {
            // Quoted standalone phrase: "annual report 2024"
            ++i;
            j = i;
            while (j < text.size() && text[j] != L'"') ++j;
            raw = text.substr(i, j - i);
            i = (j < text.size() && text[j] == L'"') ? j + 1 : j;
            raw = trim(raw);
            if (!raw.empty()) {
                q.tokens.push_back(fold(raw));
            }
            continue;
        } else {
            // Find end of token, respecting quotes like name:"annual report"
            bool inQuotes = false;
            while (j < text.size()) {
                if (text[j] == L'"') inQuotes = !inQuotes;
                else if (iswspace(text[j]) && !inQuotes) break;
                ++j;
            }
            raw = text.substr(i, j - i);
            i = j;
        }

        std::wstring lowerRaw = fold(raw), rest;
        if (lowerRaw.compare(0, 10, L"pathregex:") == 0 && raw.size() > 10) { tryRegex(stripEnclosingQuotes(raw.substr(10)), q.pathRegexes); continue; }
        if (lowerRaw.compare(0, 6, L"regex:") == 0 && raw.size() > 6) { tryRegex(stripEnclosingQuotes(raw.substr(6)), q.nameRegexes); continue; }

        const std::wstring& tok = lowerRaw;
        if (std::vector<std::wstring> exts; parseExtToken(tok, exts)) {
            q.exts.insert(q.exts.end(), exts.begin(), exts.end());
            continue;
        }
        if (startsWith(tok, L"name:", &rest) || startsWith(tok, L"ad:", &rest) || startsWith(tok, L"isim:", &rest)) {
            rest = stripEnclosingQuotes(rest);
            if (!rest.empty()) { q.names.push_back(rest); continue; }
        }
        if (startsWith(tok, L"path:", &rest) || startsWith(tok, L"yol:", &rest)) {
            rest = stripEnclosingQuotes(rest);
            if (!rest.empty()) { q.paths.push_back(rest); continue; }
        }
        if (startsWith(tok, L"folder:", &rest) || startsWith(tok, L"klasor:", &rest)) {
            rest = stripEnclosingQuotes(rest);
            if (!rest.empty()) { q.folders.push_back(rest); continue; }
        }
        if (startsWith(tok, L"scope:", &rest) || startsWith(tok, L"konum:", &rest) || startsWith(tok, L"yer:", &rest)) {
            std::wstring s = fold(stripEnclosingQuotes(rest));
            if (s == L"desktop" || s == L"masaustu") q.scope = SCOPE_DESKTOP;
            else if (s == L"downloads" || s == L"indirilenler") q.scope = SCOPE_DOWNLOADS;
            else if (s == L"documents" || s == L"docs" || s == L"belgeler") q.scope = SCOPE_DOCUMENTS;
            else if (s == L"user" || s == L"kullanici") q.scope = SCOPE_USER_AREA;
            else if (s == L"pictures" || s == L"resimler") q.scope = SCOPE_PICTURES;
            else if (s == L"music" || s == L"muzik") q.scope = SCOPE_MUSIC;
            else if (s == L"videos" || s == L"videolar") q.scope = SCOPE_VIDEOS;
            else if (s == L"c") q.scope = SCOPE_DRIVE_C;
            else if (s == L"d") q.scope = SCOPE_DRIVE_D;
            continue;
        }
        if (startsWith(tok, L"size:", &rest) || startsWith(tok, L"boyut:", &rest)) {
            int64_t a, b;
            if (parseSizeRange(stripEnclosingQuotes(rest), a, b)) { q.sizeMin = a; q.sizeMax = b; continue; }
        }
        if (startsWith(tok, L"date:", &rest) || startsWith(tok, L"tarih:", &rest)) {
            int64_t a, b;
            if (parseDateRange(stripEnclosingQuotes(rest), now, a, b)) { q.dateMin = a; q.dateMax = b; continue; }
        }
        if (tok.size() > 1 && tok[0] == L'~') { q.fuzzy.push_back(stripEnclosingQuotes(tok.substr(1))); continue; }
        if (tok.find(L'|') != std::wstring::npos) {
            std::vector<std::wstring> group;
            for (auto& p : split(tok, L'|')) {
                p = stripEnclosingQuotes(p);
                if (!p.empty()) group.push_back(p);
            }
            if (group.size() > 1) { q.alternates.push_back(std::move(group)); continue; }
        }
        if (containsAny(tok, L"*?")) { q.globs.push_back(stripEnclosingQuotes(tok)); continue; }
        std::wstring cleanTok = stripEnclosingQuotes(tok);
        if (!cleanTok.empty()) q.tokens.push_back(cleanTok);
    }
    for (size_t k = 0; k < q.tokens.size(); ++k) q.phrase += (k ? L" " : L"") + q.tokens[k];
    return q;
}

static bool matchAnyPart(std::wstring_view s, const std::wstring& value) {
    size_t start = 0;
    for (;;) {
        size_t p = value.find(L'|', start);
        std::wstring_view part(value.data() + start, (p == std::wstring::npos ? value.size() : p) - start);
        if (!part.empty() && s.find(part) != std::wstring_view::npos) return true;
        if (p == std::wstring::npos) return false;
        start = p + 1;
    }
}

static bool isSubsequence(const std::wstring& needle, std::wstring_view hay) {
    size_t i = 0;
    for (size_t j = 0; j < hay.size() && i < needle.size(); ++j)
        if (hay[j] == needle[i]) ++i;
    return i == needle.size();
}

static bool regexFind(const std::wregex& re, std::wstring_view s) {
    try {
        return std::regex_search(s.begin(), s.end(), re);
    } catch (...) {
        return false;
    }
}

bool matchesCategory(const Entry& e, FileCategory cat) {
    if (cat == CAT_ALL) return true;
    if (cat == CAT_FOLDERS) return e.isDir;
    if (e.isDir) return false;
    std::wstring extStr = e.lowerExt();
    std::wstring_view ext = extStr;
    if (!ext.empty() && ext.front() == L'.') ext.remove_prefix(1);
    switch (cat) {
        case CAT_DOCS: {
            static const wchar_t* exts[] = {L"pdf", L"doc", L"docx", L"txt", L"xls", L"xlsx", L"ppt", L"pptx", L"odt", L"csv", L"md", L"rtf", L"log", L"epub"};
            for (auto* x : exts) if (ext == x) return true;
            return false;
        }
        case CAT_IMAGES: {
            static const wchar_t* exts[] = {L"jpg", L"jpeg", L"png", L"gif", L"bmp", L"webp", L"svg", L"ico", L"tiff", L"psd", L"raw"};
            for (auto* x : exts) if (ext == x) return true;
            return false;
        }
        case CAT_AUDIO: {
            static const wchar_t* exts[] = {L"mp3", L"wav", L"flac", L"aac", L"ogg", L"m4a", L"wma", L"opus", L"mid", L"alac"};
            for (auto* x : exts) if (ext == x) return true;
            return false;
        }
        case CAT_VIDEO: {
            static const wchar_t* exts[] = {L"mp4", L"mkv", L"avi", L"mov", L"wmv", L"flv", L"webm", L"m4v", L"3gp", L"ts", L"mpg"};
            for (auto* x : exts) if (ext == x) return true;
            return false;
        }
        case CAT_ARCHIVES: {
            static const wchar_t* exts[] = {L"zip", L"rar", L"7z", L"tar", L"gz", L"bz2", L"iso", L"xz", L"cab", L"tgz", L"zst"};
            for (auto* x : exts) if (ext == x) return true;
            return false;
        }
        case CAT_APPS: {
            static const wchar_t* exts[] = {L"exe", L"msi", L"bat", L"cmd", L"ps1", L"lnk", L"appref-ms"};
            for (auto* x : exts) if (ext == x) return true;
            return false;
        }
        default: return true;
    }
}

bool matchesScope(const Entry& e, ScopeType scope) {
    if (scope == SCOPE_ALL) return true;
    const auto& sp = getScopePaths();
    std::wstring_view dir = e.lowerDir();
    switch (scope) {
        case SCOPE_DESKTOP:
            return !sp.desktop.empty() && dir.starts_with(sp.desktop);
        case SCOPE_DOWNLOADS:
            return !sp.downloads.empty() && dir.starts_with(sp.downloads);
        case SCOPE_DOCUMENTS:
            return !sp.documents.empty() && dir.starts_with(sp.documents);
        case SCOPE_USER_AREA:
            return (!sp.desktop.empty() && dir.starts_with(sp.desktop)) ||
                   (!sp.downloads.empty() && dir.starts_with(sp.downloads)) ||
                   (!sp.documents.empty() && dir.starts_with(sp.documents));
        case SCOPE_PICTURES:
            return !sp.pictures.empty() && dir.starts_with(sp.pictures);
        case SCOPE_MUSIC:
            return !sp.music.empty() && dir.starts_with(sp.music);
        case SCOPE_VIDEOS:
            return !sp.videos.empty() && dir.starts_with(sp.videos);
        case SCOPE_DRIVE_C:
            return dir.starts_with(L"c:");
        case SCOPE_DRIVE_D:
            return dir.starts_with(L"d:");
        default:
            return true;
    }
}

bool matches(const Entry& e, const Query& q) {
    if (q.browseDirectOnly) {
        std::wstring_view a = e.lowerDir();
        std::wstring b = fold(q.browseDir);
        std::wstring_view bv = b;
        while (a.size() > 3 && (a.back() == L'\\' || a.back() == L'/')) a.remove_suffix(1);
        while (bv.size() > 3 && (bv.back() == L'\\' || bv.back() == L'/')) bv.remove_suffix(1);
        if (a != bv) return false;
        if (!matchesCategory(e, q.category)) return false;
        if (q.emptyTokens()) return true;
    } else {
        if (!matchesScope(e, q.scope)) return false;
        if (!matchesCategory(e, q.category)) return false;
    }
    std::wstring lname = e.lowerName();
    std::wstring_view ldir = e.lowerDir();
    if (!q.exts.empty()) {
        std::wstring ext = e.lowerExt();
        if (ext.size() < 2) return false;
        std::wstring_view extView = ext;
        extView.remove_prefix(1);
        bool ok = false;
        for (auto& w : q.exts) if (extView == w) { ok = true; break; }
        if (!ok) return false;
    }
    for (auto& w : q.names) if (!matchAnyPart(lname, w)) return false;
    for (auto& w : q.folders) {
        std::wstring_view target = e.isDir ? lname : ldir;
        if (!matchAnyPart(target, w)) return false;
    }
    for (auto& g : q.globs) if (!wildMatch(g, lname)) return false;

    // For full path queries (path: or tokens or regex on path)
    bool needPath = !q.paths.empty() || !q.pathRegexes.empty() || !q.tokens.empty() || !q.alternates.empty() || !q.fuzzy.empty();
    std::wstring lpathStorage;
    std::wstring_view lpath;
    if (needPath) {
        lpathStorage = e.lower();
        lpath = lpathStorage;
    }

    for (auto& w : q.paths) if (!matchAnyPart(lpath, w)) return false;
    for (auto& group : q.alternates) {
        bool ok = false;
        for (auto& alt : group) if (lpath.find(alt) != std::wstring_view::npos) { ok = true; break; }
        if (!ok) return false;
    }
    for (auto& f : q.fuzzy) if (!isSubsequence(f, lname) && !isSubsequence(f, lpath)) return false;
    if (q.sizeMin >= 0 && e.size < q.sizeMin) return false;
    if (q.sizeMax >= 0 && e.size > q.sizeMax) return false;
    if (q.dateMin >= 0 && e.mtime < q.dateMin) return false;
    if (q.dateMax >= 0 && e.mtime > q.dateMax) return false;
    for (auto& t : q.tokens) {
        if (lname.find(t) != std::wstring_view::npos) continue;
        if (lpath.find(t) == std::wstring_view::npos) return false;
    }
    for (auto& re : q.nameRegexes) if (!regexFind(re, lname)) return false;
    for (auto& re : q.pathRegexes) if (!regexFind(re, lpath)) return false;
    return true;
}

int score(const Entry& e, const Query& q) {
    if (q.browseDirectOnly) {
        return e.isDir ? -10000 : 0;
    }
    std::wstring_view dir = e.dir();
    int depth = (int)std::count(dir.begin(), dir.end(), L'\\');
    int sc = e.priority * 1000 + depth * 2;
    if (e.isDir) sc -= 80;
    std::wstring lname = e.lowerName();
    std::wstring ext = e.lowerExt();
    std::wstring_view extView = ext;
    std::wstring_view extNoDot = extView.empty() ? extView : extView.substr(1);
    if (extNoDot == L"lnk" || extNoDot == L"url" || extNoDot == L"exe" || extNoDot == L"appref-ms") sc -= 60;
    else if (extNoDot == L"dll" || extNoDot == L"sys" || extNoDot == L"tmp" || extNoDot == L"log" ||
             extNoDot == L"cache" || extNoDot == L"dat" || extNoDot == L"pak" || extNoDot == L"manifest") sc += 180;

    // --- Akıllı Konum Önceliği: Kullanıcı odaklı alanlar en üstte listelenir ---
    const auto& sp = getScopePaths();
    std::wstring_view ldir = e.lowerDir();
    if (!sp.desktop.empty() && ldir.starts_with(sp.desktop)) {
        sc -= 450; // Masaüstü dosyaları en üstte
    } else if (!sp.downloads.empty() && ldir.starts_with(sp.downloads)) {
        sc -= 380; // İndirilenler
    } else if (!sp.documents.empty() && ldir.starts_with(sp.documents)) {
        sc -= 320; // Belgeler
    } else if (!sp.userProfile.empty() && ldir.starts_with(sp.userProfile)) {
        if (ldir.find(L"\\appdata\\") == std::wstring_view::npos) {
            sc -= 180; // Kullanıcı profilindeki aktif projeler/dosyalar
        }
    }

    // --- Güncellik (Recency) Önceliği: Yakın zamanda değiştirilen dosyalar üstte ---
    int64_t nowSec = static_cast<int64_t>(std::time(nullptr));
    int64_t ageSec = nowSec - e.mtime;
    if (ageSec >= 0) {
        if (ageSec < 86400) {            // Son 24 saat
            sc -= 160;
        } else if (ageSec < 7 * 86400) {   // Son 7 gün
            sc -= 80;
        } else if (ageSec < 30 * 86400) {  // Son 30 gün
            sc -= 30;
        }
    }
    if (!q.phrase.empty()) {
        std::wstring_view stem = lname.substr(0, lname.size() - ext.size());
        std::wstring_view ph = q.phrase;
        auto has = [&](std::wstring_view h) { return h.find(ph) != std::wstring_view::npos; };
        auto pre = [&](std::wstring_view h) { return h.substr(0, ph.size()) == ph; };
        bool allInName = true;
        for (auto& t : q.tokens) if (lname.find(t) == std::wstring_view::npos) { allInName = false; break; }
        if (lname == ph || stem == ph) sc -= 320;
        else if (pre(stem) || pre(lname)) sc -= 220;
        else if (has(stem) || has(lname)) sc -= 130;
        else if (allInName) sc -= 90;
    }
    for (auto& w : q.exts) if (extNoDot == w) { sc -= 120; break; }
    for (auto& w : q.names) {
        if (lname == w) sc -= 320;
        else if (lname.substr(0, w.size()) == w) sc -= 220;
        else if (lname.find(w) != std::wstring_view::npos) sc -= 130;
    }
    if (ldir.find(L"\\setup\\") != std::wstring_view::npos || ldir.find(L"\\installer\\") != std::wstring_view::npos) sc += 120;
    return sc;
}

namespace {
struct Ranked {
    size_t idx;
    int score;
};
}  // namespace

bool searchEntries(const std::vector<Entry>& entries, const Query& q, size_t limit,
                   const std::atomic<bool>& cancel, std::vector<size_t>& out) {
    out.clear();
    if (limit == 0 || q.empty()) return true;
    auto better = [&](const Ranked& a, const Ranked& b) {
        if (a.score != b.score) return a.score < b.score;
        int cmp = _wcsicmp(entries[a.idx].name.c_str(), entries[b.idx].name.c_str());
        if (cmp != 0) return cmp < 0;
        return a.idx < b.idx;
    };
    auto heapCmp = [&](const Ranked& a, const Ranked& b) { return better(a, b); };
    std::vector<Ranked> heap;
    heap.reserve(std::min<size_t>(limit, 4096));
    for (size_t i = 0; i < entries.size(); ++i) {
        if ((i & 0x3FF) == 0 && cancel.load(std::memory_order_relaxed)) return false;
        const Entry& e = entries[i];
        if (!matches(e, q)) continue;
        Ranked r{i, score(e, q)};
        if (heap.size() < limit) {
            heap.push_back(r);
            std::push_heap(heap.begin(), heap.end(), heapCmp);
        } else if (better(r, heap.front())) {
            std::pop_heap(heap.begin(), heap.end(), heapCmp);
            heap.back() = r;
            std::push_heap(heap.begin(), heap.end(), heapCmp);
        }
    }
    std::sort(heap.begin(), heap.end(), better);
    out.reserve(heap.size());
    for (auto& r : heap) out.push_back(r.idx);
    return true;
}

}  // namespace qb
