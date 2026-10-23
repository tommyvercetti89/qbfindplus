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
    CHECK(fold(L"kâğıt millî sükût") == L"kagit milli sukut");
    CHECK(wildMatch(L"rep*.pdf", L"report.pdf"));
    CHECK(wildMatch(L"r?port*", L"report.pdf"));
    CHECK(!wildMatch(L"rep*.doc", L"report.pdf"));

    // sizes
    int64_t a, b;
    CHECK(parseSize(L"10mb", a) && a == 10LL * 1048576);
    CHECK(parseSize(L"10m", a) && a == 10LL * 1048576);
    CHECK(parseSize(L"500k", a) && a == 500LL * 1024);
    CHECK(parseSize(L"2g", a) && a == 2LL * 1073741824);
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

    // quoted phrase and turkish filter keywords parsing
    Query qPhrase = parseQuery(L"\"annual report\" name:\"financial plan\" folder:\"my documents\"");
    CHECK(qPhrase.tokens.size() == 1 && qPhrase.tokens[0] == L"annual report");
    CHECK(qPhrase.names.size() == 1 && qPhrase.names[0] == L"financial plan");
    CHECK(qPhrase.folders.size() == 1 && qPhrase.folders[0] == L"my documents");

    Query qTr = parseQuery(L"ext:pdf,docx,xlsx boyut:10mb..100mb ad:rapor yol:belgeler tarih:bugun");
    CHECK(qTr.exts.size() == 3);
    CHECK(qTr.sizeMin == 10485760 && qTr.sizeMax == 104857600);
    CHECK(qTr.names.size() == 1 && qTr.names[0] == L"rapor");
    CHECK(qTr.paths.size() == 1 && qTr.paths[0] == L"belgeler");
    CHECK(qTr.dateMin > 0 && qTr.dateMax > 0);

    // matching
    Entry pdf = E(L"C:\\Users\\User\\Belgeler\\Rapor.PDF", 5000);
    Entry txt = E(L"C:\\Temp\\rapor.txt", 10);
    Entry notBelgeler = E(L"C:\\Users\\User\\Diger\\Belgeler.txt", 100);
    CHECK(matches(pdf, parseQuery(L"rapor")));
    CHECK(matches(pdf, parseQuery(L".pdf")));
    CHECK(matches(pdf, parseQuery(L"ext:pdf,docx")));
    CHECK(matches(pdf, parseQuery(L"ad:rapor")));
    CHECK(matches(pdf, parseQuery(L"boyut:>1kb")));
    CHECK(!matches(txt, parseQuery(L"ext:pdf")));
    CHECK(matches(pdf, parseQuery(L"BELGELER rapor")));
    CHECK(matches(pdf, parseQuery(L"name:rapor path:user")));
    CHECK(matches(pdf, parseQuery(L"folder:Belgeler")));
    CHECK(!matches(notBelgeler, parseQuery(L"folder:Belgeler"))); // matches directory part, not filename
    CHECK(matches(pdf, parseQuery(L"size:>1kb")));
    CHECK(!matches(txt, parseQuery(L"size:>1kb")));
    CHECK(matches(pdf, parseQuery(L"rap*.pdf")));
    CHECK(matches(pdf, parseQuery(L"txt|pdf")));
    CHECK(matches(pdf, parseQuery(L"~rpr")));
    CHECK(matches(pdf, parseQuery(L"regex:^rap.r\\.pdf$")));
    CHECK(!matches(pdf, parseQuery(L"regex:^xyz")));

    // lazy regex quantifier (*?, +?) support
    Query qLazy = parseQuery(L"regex:rap.*?pdf");
    CHECK(qLazy.nameRegexes.size() == 1);
    CHECK(matches(pdf, qLazy));

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

    // category tests
    Entry doc = E(L"C:\\Docs\\notes.docx");
    Entry img = E(L"C:\\Pics\\photo.png");
    Entry aud = E(L"C:\\Music\\song.mp3");
    Entry vid = E(L"C:\\Movies\\clip.mp4");
    Entry arc = E(L"C:\\Archives\\backup.zip");
    Entry app = E(L"C:\\Tools\\util.exe");
    Entry dirEntry = E(L"C:\\Docs\\Folder", 0, 0, true);

    CHECK(matchesCategory(doc, CAT_DOCS));
    CHECK(!matchesCategory(doc, CAT_IMAGES));
    CHECK(matchesCategory(img, CAT_IMAGES));
    CHECK(matchesCategory(aud, CAT_AUDIO));
    CHECK(matchesCategory(vid, CAT_VIDEO));
    CHECK(matchesCategory(arc, CAT_ARCHIVES));
    CHECK(matchesCategory(app, CAT_APPS));
    CHECK(matchesCategory(dirEntry, CAT_FOLDERS));
    CHECK(!matchesCategory(doc, CAT_FOLDERS));
    CHECK(matchesCategory(doc, CAT_ALL));

    // DirPool verification
    CHECK(doc.path() == L"C:\\Docs\\notes.docx");
    CHECK(doc.dir() == L"C:\\Docs");
    CHECK(doc.name == L"notes.docx");

    // Scope & Location filtering tests
    const auto& scopes = getScopePaths();
    if (!scopes.desktop.empty()) {
        Entry dskEntry = E((scopes.desktop + L"\\test.txt").c_str());
        CHECK(matchesScope(dskEntry, SCOPE_DESKTOP));
        CHECK(matchesScope(dskEntry, SCOPE_USER_AREA));
        CHECK(matchesScope(dskEntry, SCOPE_ALL));
        CHECK(!matchesScope(dskEntry, SCOPE_DOWNLOADS));
    }
    if (!scopes.downloads.empty()) {
        Entry dwnEntry = E((scopes.downloads + L"\\setup.exe").c_str());
        CHECK(matchesScope(dwnEntry, SCOPE_DOWNLOADS));
        CHECK(matchesScope(dwnEntry, SCOPE_USER_AREA));
        CHECK(!matchesScope(dwnEntry, SCOPE_DESKTOP));
    }
    Entry cDrive = E(L"C:\\Windows\\notepad.exe");
    CHECK(matchesScope(cDrive, SCOPE_DRIVE_C));
    CHECK(!matchesScope(cDrive, SCOPE_DRIVE_D));

    // Query scope parsing & empty check
    Query qScope1 = parseQuery(L"scope:desktop");
    CHECK(qScope1.scope == SCOPE_DESKTOP);
    CHECK(!qScope1.empty());

    Query qScope2 = parseQuery(L"konum:indirilenler");
    CHECK(qScope2.scope == SCOPE_DOWNLOADS);

    Query qScope3 = parseQuery(L"yer:belgeler");
    CHECK(qScope3.scope == SCOPE_DOCUMENTS);

    Query qScope4 = parseQuery(L"scope:user test");
    CHECK(qScope4.scope == SCOPE_USER_AREA);
    CHECK(qScope4.tokens.size() == 1 && qScope4.tokens[0] == L"test");

    // Ranking boost test: Desktop file should rank ahead of identical deep file
    if (!scopes.desktop.empty()) {
        Entry deepFile = E(L"C:\\Windows\\Temp\\work.txt", 100);
        Entry dskFile = E((scopes.desktop + L"\\work.txt").c_str(), 100);
        std::vector<Entry> rankEntries = {deepFile, dskFile};
        std::vector<size_t> rankOut;
        std::atomic<bool> rankCancel{false};
        CHECK(searchEntries(rankEntries, parseQuery(L"work"), 10, rankCancel, rankOut));
        CHECK(rankOut.size() == 2 && rankOut[0] == 1 && rankOut[1] == 0); // Desktop item comes first!
    }

    // Browse mode tests (direct children only, folders first, alphabetical)
    Entry topFolder = E(L"C:\\Users\\User\\Desktop\\Projeler", 0, 0, true);
    Entry topFileB = E(L"C:\\Users\\User\\Desktop\\zebra.txt", 100);
    Entry topFileA = E(L"C:\\Users\\User\\Desktop\\alpha.txt", 100);
    Entry subFile = E(L"C:\\Users\\User\\Desktop\\Projeler\\sub.txt", 100);
    Entry subFolder = E(L"C:\\Users\\User\\Desktop\\Projeler\\subfolder", 0, 0, true);

    Query qBrowse;
    qBrowse.browseDir = L"C:\\Users\\User\\Desktop";
    qBrowse.browseDirectOnly = true;
    CHECK(!qBrowse.empty());
    CHECK(matches(topFolder, qBrowse));
    CHECK(matches(topFileA, qBrowse));
    CHECK(matches(topFileB, qBrowse));
    CHECK(!matches(subFile, qBrowse));    // Sub-items must NOT match direct browse mode!
    CHECK(!matches(subFolder, qBrowse));

    std::vector<Entry> browseEntries = {topFileB, subFile, topFolder, topFileA, subFolder};
    std::vector<size_t> browseOut;
    std::atomic<bool> bCancel{false};
    CHECK(searchEntries(browseEntries, qBrowse, 10, bCancel, browseOut));
    // Must contain topFolder (idx 2), topFileA (idx 3), topFileB (idx 0) in that exact order!
    CHECK(browseOut.size() == 3);
    CHECK(browseOut[0] == 2); // Folders first: Projeler
    CHECK(browseOut[1] == 3); // File alpha.txt
    CHECK(browseOut[2] == 0); // File zebra.txt

    std::printf(failures ? "%d FAILURES\n" : "all tests passed\n", failures);
    return failures ? 1 : 0;
}
