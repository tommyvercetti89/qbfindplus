// Minimal dependency-free tests for the query engine.
#include <cstdio>
#include <ctime>

#include "../src/search.h"

using namespace qb;

static int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

static Entry E(const wchar_t* path, int64_t size = 0, int64_t mtime = 0, bool dir = false, int prio = 3) {
    return makeEntry(path, size, mtime, dir, prio);
}

int main() {
    // folding
    CHECK(fold(L"ÇAĞRI İşıl ÖÜ") == L"cagri isil ou");
    CHECK(wildMatch(L"rep*.pdf", L"report.pdf"));
    CHECK(wildMatch(L"r?port*", L"report.pdf"));
    CHECK(!wildMatch(L"rep*.doc", L"report.pdf"));

    // sizes
    int64_t a, b;
    CHECK(parseSize(L"10mb", a) && a == 10LL * 1048576);
    CHECK(parseSize(L"1.5kb", a) && a == 1536);
    CHECK(!parseSize(L"abc", a));
    CHECK(parseSizeRange(L">10mb", a, b) && a == 10LL * 1048576 + 1 && b == -1);
    CHECK(parseSizeRange(L"1mb-10mb", a, b) && a == 1048576 && b == 10LL * 1048576);
    CHECK(!parseSizeRange(L"10mb-1mb", a, b));

    // dates
    std::time_t now = std::time(nullptr);
    CHECK(parseDateRange(L"today", now, a, b) && a <= now && b >= now);
    CHECK(parseDateRange(L">2024-01-01", now, a, b) && b == -1 && a > 0);
    CHECK(parseDateRange(L"2024", now, a, b) && b > a);
    CHECK(!parseDateRange(L"garbage", now, a, b));

    // parsing
    Query q = parseQuery(L"ext:pdf|docx report* size:>1kb ~abc regex:(");
    CHECK(q.exts.size() == 2);
    CHECK(q.globs.size() == 1);
    CHECK(q.fuzzy.size() == 1);
    CHECK(q.nameRegexes.empty());  // invalid regex is ignored, not fatal
    CHECK(q.sizeMin == 1025);
    CHECK(!q.empty());
    CHECK(parseQuery(L"   ").empty());

    // matching
    Entry pdf = E(L"C:\\Users\\User\\Belgeler\\Rapor.PDF", 5000);
    Entry txt = E(L"C:\\Temp\\rapor.txt", 10);
    CHECK(matches(pdf, parseQuery(L"rapor")));
    CHECK(matches(pdf, parseQuery(L".pdf")));
    CHECK(!matches(txt, parseQuery(L"ext:pdf")));
    CHECK(matches(pdf, parseQuery(L"BELGELER rapor")));
    CHECK(matches(pdf, parseQuery(L"name:rapor path:user")));
    CHECK(matches(pdf, parseQuery(L"size:>1kb")));
    CHECK(!matches(txt, parseQuery(L"size:>1kb")));
    CHECK(matches(pdf, parseQuery(L"rap*.pdf")));
    CHECK(matches(pdf, parseQuery(L"txt|pdf")));
    CHECK(matches(pdf, parseQuery(L"~rpr")));
    CHECK(matches(pdf, parseQuery(L"regex:^rap.r\\.pdf$")));
    CHECK(!matches(pdf, parseQuery(L"regex:^xyz")));

    // ranking: exact name beats deep match; priority roots beat noisy ones
    std::vector<Entry> es = {E(L"C:\\Windows\\deep\\a\\b\\report.dll", 1, 0, false, 5), E(L"C:\\Users\\User\\Desktop\\report.pdf", 1, 0, false, 0),
                             E(L"C:\\Users\\User\\Desktop\\my report notes.txt", 1, 0, false, 0), E(L"C:\\Data\\other.txt")};
    std::atomic<bool> cancel{false};
    std::vector<size_t> out;
    CHECK(searchEntries(es, parseQuery(L"report"), 10, cancel, out));
    CHECK(out.size() == 3 && out[0] == 1 && out[1] == 2 && out[2] == 0);
    CHECK(searchEntries(es, parseQuery(L"report"), 1, cancel, out) && out.size() == 1 && out[0] == 1);

    // cancellation
    cancel = true;
    std::vector<Entry> many(5000, E(L"C:\\x\\report.txt"));
    CHECK(!searchEntries(many, parseQuery(L"report"), 10, cancel, out));

    // regex safety checks (ReDoS protection)
    std::wstring evilNested(L"regex:(a+)+");
    Query qEvil = parseQuery(evilNested);
    CHECK(qEvil.nameRegexes.empty()); // Rejected nested quantifiers

    std::wstring overlyLong = L"regex:" + std::wstring(150, L'x');
    Query qLong = parseQuery(overlyLong);
    CHECK(qLong.nameRegexes.empty()); // Rejected over-length regex pattern

    // history & saved searches
    clearHistory();
    addHistory(L"test query 1");
    addHistory(L"test query 2");
    auto h = loadHistory();
    CHECK(h.size() == 2);
    CHECK(h[0] == L"test query 2"); // most recent first
    clearHistory();
    CHECK(loadHistory().empty());

    // settings load/save
    Settings s;
    s.lang = L"tr";
    s.darkMode = true;
    s.excludes = {L"node_modules", L".git"};
    CHECK(saveSettings(s));
    Settings loaded = loadSettings();
    CHECK(loaded.lang == L"tr");
    CHECK(loaded.darkMode == true);
    CHECK(loaded.excludes.size() == 2);

    std::printf(failures ? "%d FAILURES\n" : "all tests passed\n", failures);
    return failures ? 1 : 0;
}
